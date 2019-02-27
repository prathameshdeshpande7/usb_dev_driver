#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#define USB_SKEL_MINOR_BASE	192

/* Define these values to match your devices */
#define PD_VENDOR_ID	0x03f0
#define PD_PRODUCT_ID	0x171d


/* table of devices that work with this driver */ 
static struct usb_device_id pd_table [] = {
    { USB_DEVICE(PD_VENDOR_ID, PD_PRODUCT_ID) },
    { }		/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, pd_table);


 #define MAX_TRANSFER        (PAGE_SIZE - 512)  
 #define WRITES_IN_FLIGHT    8  


struct usb_pd{
    /* One structure for each connected device */
    	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	 struct semaphore    limit_sem;      /* limiting the number of writes in progress */  
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	unsigned char           *bulk_in_buffer;	/* the buffer to receive data */
	size_t			bulk_in_size;		/* the size of the receive buffer */
	size_t			bulk_in_filled;		/* number of bytes in the buffer */
	size_t			bulk_in_copied;		/* already copied to user space */
	__u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
	__u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
	struct completion	bulk_in_completion;	/* to wait for an ongoing read */
	struct kref     kref;  
  	struct mutex        io_mutex;       /* synchronize I/O with disconnect */  

};

#define to_skel_dev(d) container_of(d, struct usb_pd, kref)  

static struct usb_driver pd_driver;

static void pd_delete(struct kref *kref)
{
	struct usb_pd *dev = to_skel_dev(kref);

	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}


static int pd_open(struct inode *inode, struct file *file)
{
	struct usb_pd *dev;
	struct usb_interface *interface;
	int subminor;
	int retval=0;

	subminor = iminor(inode);
	
	interface = usb_find_interface(&pd_driver, subminor);
	if(!interface)
	{
		printk ("%s - error, can't find device for minor %d",__FUNCTION__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if(!dev)
	{
		retval = -ENODEV;
		goto exit;
	}
	
	/*prevent the device from getting autosuspended */
	retval = usb_autopm_get_interface(interface);
	if(retval)
		goto exit;

	/* increment our usage count for the device */
	kref_get(&dev->kref);

	/* save our object in the file's private structure */
	file->private_data=dev;
exit:
	return retval;
	
	//return 0;

    /* open syscall */
}
static int pd_release(struct inode *inode, struct file *file)
{
	struct usb_pd *dev;

	dev = (struct usb_pd *)file->private_data;
	if (dev==NULL)
		return -ENODEV;
	/* allow the device to be autosuspended */  
	mutex_lock(&dev->io_mutex);  
	if (dev->interface)  
		 usb_autopm_put_interface(dev->interface);
  	mutex_unlock(&dev->io_mutex);  
	    
       /* decrement the count on our device */  
	    kref_put(&dev->kref, pd_delete); 

	return 0;
    /* close syscall */
}

static ssize_t pd_read(struct file *file, char *buffer, size_t count, loff_t *ppos)
{
	struct usb_pd *dev;
	int retval;
	int bytes_read;
	
	dev = (struct usb_pd *)file->private_data;

	mutex_lock(&dev->io_mutex);
	if(!dev->interface)
	{
		/*disconnect() was called */
		retval = -ENODEV;
		goto exit;
	}

	/* do a blocking bulk read to get data from the device */
	retval = usb_bulk_msg(dev->udev, usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr),
			dev->bulk_in_buffer,
			min(dev->bulk_in_size, count),
			&bytes_read, 10000);

	/* if the read was successful, copy the data to userspace */
	if (!retval)
	{
		if(copy_to_user(buffer, dev->bulk_in_buffer, bytes_read))
			retval = -EFAULT;
		else
			retval = bytes_read;
	}

exit:
	mutex_unlock(&dev->io_mutex);
	return retval;
}

static void pd_write_bulk_callback(struct urb *urb)
{
	struct usb_pd *dev;
	
	dev = (struct usb_pd *)urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status &&
		!(urb->status == -ENOENT ||
		  urb->status == -ECONNRESET ||
		  urb->status == -ESHUTDOWN))
	{
		printk("%s - nonzero write bulk status received: %d",__FUNCTION__, urb->status);
	}
	
	/* free up our allocated buffer */
	usb_free_coherent (urb->dev, urb->transfer_buffer_length,urb->transfer_buffer, urb->transfer_dma);
	up(&dev->limit_sem);
}


static ssize_t pd_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *ppos)
{
	struct usb_pd *dev;
	int retval = 0;
	struct urb *urb = NULL;
	char * buf= NULL;
	size_t writesize = min(count, (size_t)MAX_TRANSFER);
	
	dev = (struct usb_pd *)file->private_data;

	/*verify that we actually have some data to write */
	if(count == 0)
		goto exit;

	/* limit the number of URBs in flight to stop a user from using up all RAM */
	if(down_interruptible(&dev->limit_sem))
	{
		retval = -ERESTARTSYS;
		goto exit;
	}
	
	mutex_lock(&dev->io_mutex);
	if(!dev->interface)	
	{
		/* disconnect() was called */
		retval = -ENODEV;
		goto error;
	}
	
	/* create a urb , and a buffer for, and copy the data to the urb */
	
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
	{
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(dev->udev, writesize, GFP_KERNEL, &urb->transfer_dma);
	if (!buf)
	{
		retval = -ENOMEM;
		goto error;
	}
	
	if (copy_from_user(buf, user_buffer, writesize))
	{
		retval = -EFAULT;
		goto error;
	}
	
	/* initialize the urb properly */
	usb_fill_bulk_urb(urb, dev->udev, usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
	buf, writesize, pd_write_bulk_callback, dev);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	/* send the data out the bulk port */
	retval = usb_submit_urb(urb, GFP_KERNEL);
	if(retval)
	{
		printk("%s - failed submitting write urb, error %d",__FUNCTION__,retval);
	goto error;
	}

	/* release our reference to this urb, the USB core will eventually free it entirely */
	usb_free_urb(urb);
	
	mutex_unlock(&dev->io_mutex);
	return writesize;

error:
	if (urb)
	{
		usb_free_coherent(dev->udev, writesize, buf, urb->transfer_dma);
		usb_free_urb(urb);
	}
	mutex_unlock(&dev->io_mutex);
	up(&dev->limit_sem);

exit:
	return retval;
	
	
    /* write syscall */
}

static const struct file_operations pd_fops = {
    .owner =    THIS_MODULE,
    .read  =	pd_read,
    .write =    pd_write,
    .open =     pd_open,
    .release =  pd_release,
};
/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver pd_class = {
	.name =		"prathamesh driver",
	.fops =		&pd_fops,
	.minor_base =	USB_SKEL_MINOR_BASE,
};

static int pd_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    /* called when a USB device is connected to the computer. */
	struct usb_pd *dev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
        size_t buffer_size;
	int i;
	int retval = -ENOMEM;

	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		printk("Out of memory");
		goto error;
	}

        kref_init(&dev->kref);  
        sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);  
        mutex_init(&dev->io_mutex);
 
	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = interface;

	/* set up the endpoint information */
	/* use only the first bulk-in and bulk-out endpoints */
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i)
	 {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!dev->bulk_in_endpointAddr &&
		(endpoint->bEndpointAddress & USB_DIR_IN) &&
		((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
		== USB_ENDPOINT_XFER_BULK))

		 //    usb_endpoint_is_bulk_in(endpoint))
		 {
			/* we found a bulk in endpoint */
			buffer_size = le16_to_cpu(endpoint->wMaxPacketSize);
			dev->bulk_in_size = buffer_size;
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
			printk(KERN_ALERT "\nOK bulk in endpoint\n");
			if (!dev->bulk_in_buffer)
			 {
				printk("Could not allocate bulk_in_buffer");
				goto error;
			}
			//printk(KERN_ALERT "OK2\n");
			dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
			if (!dev->bulk_in_urb) 
			{
				printk("Could not allocate bulk_in_urb");
				goto error;
			}
			//printk(KERN_ALERT "OK3\n");
		}

		if (!dev->bulk_out_endpointAddr &&
		!(endpoint->bEndpointAddress & USB_DIR_IN) &&
		((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
		== USB_ENDPOINT_XFER_BULK)) 

		//    usb_endpoint_is_bulk_out(endpoint))
		 {
			/* we found a bulk out endpoint */
			printk(KERN_ALERT "\nBulk out endpoint\n");
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
		}
	} //end of for loop
	
	if (!(dev->bulk_in_endpointAddr && dev->bulk_out_endpointAddr)) 
	{
		printk(KERN_ALERT "\nTHIS IS THE POINT OF ERROR\n");
		printk("Could not find both bulk-in and bulk-out endpoints");
		goto error;
	}

     	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);
	
	/* we can register the device now, as it is ready */
	retval = usb_register_dev(interface, &pd_class);
	if (retval) {
		/* something prevented us from registering this driver */
		printk("Not able to get a minor for this device.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

      	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,"USB device now attached to prathamesh's driver-%d",interface->minor);
	return 0;

error:
	if (dev)
		 kref_put(&dev->kref, pd_delete);
	return retval;


}

static void pd_disconnect(struct usb_interface *interface)
{
    /* called when unplugging a USB device. */
	struct usb_pd *dev;
	int minor = interface->minor;
	
	 /* prevent pd_open() from racing pd_disconnect() */  
 	     //lock_kernel(); 

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* give back our minor */
	usb_deregister_dev(interface, &pd_class);
	
	/* prevent more I/O from starting */  
 	     mutex_lock(&dev->io_mutex);
		dev->interface = NULL;
	     mutex_unlock(&dev->io_mutex);	

	//unlock_kernel();

	/* decrement our usage count */  
	 kref_put(&dev->kref, pd_delete);
	
	dev_info(&interface->dev, "USB Skeleton #%d now disconnected", minor);


}

static struct usb_driver pd_driver = {
    .name = "prathamesh driver",
    .id_table = pd_table,
    .probe = pd_probe,
    .disconnect = pd_disconnect,
    
};

static int __init usb_pd_init(void)
{
 /* called on module loading */
     int result;
     /* register this driver with the USB subsystem */
    result = usb_register(&pd_driver);
    if (result)
        printk("usb_register failed. Error number %d", result);
     return result;
}

static void __exit usb_pd_exit(void)
{
    /* called on module unloading */
    usb_deregister(&pd_driver);
}

module_init(usb_pd_init);
module_exit(usb_pd_exit);

MODULE_AUTHOR("Prathamesh Deshpande");
MODULE_LICENSE("GPL");
